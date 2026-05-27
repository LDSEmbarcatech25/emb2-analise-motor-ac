const express = require('express');
const cors = require('cors');
const bcrypt = require('bcrypt');
const jwt = require('jsonwebtoken');
const { PrismaClient } = require('@prisma/client');
const path = require('path');
require('dotenv').config();

const app = express();
const prisma = new PrismaClient({});
const PORT = process.env.PORT || 3000;
const JWT_SECRET = process.env.JWT_SECRET || 'fallback_secret';

app.use(cors());
app.use(express.json());

// Serve o dashboard estático em desenvolvimento local
app.use(express.static(path.join(__dirname, '../dashboard')));

// Seed default users se não existirem
async function seedDatabase() {
    try {
        const adminCount = await prisma.user.count({ where: { email: 'admin@kiira.com' } });
        if (adminCount === 0) {
            const adminPassword = await bcrypt.hash('admin123', 10);
            await prisma.user.create({
                data: { email: 'admin@kiira.com', password: adminPassword, role: 'admin' }
            });
            console.log('✅ Admin user created: admin@kiira.com / admin123');
        }

        const operatorCount = await prisma.user.count({ where: { email: 'operador@kiira.com' } });
        if (operatorCount === 0) {
            const operatorPassword = await bcrypt.hash('operador123', 10);
            await prisma.user.create({
                data: { email: 'operador@kiira.com', password: operatorPassword, role: 'operator' }
            });
            console.log('✅ Operator user created: operador@kiira.com / operador123');
        }
        
        const viewerCount = await prisma.user.count({ where: { email: 'viewer@kiira.com' } });
        if (viewerCount === 0) {
            const viewerPassword = await bcrypt.hash('viewer123', 10);
            await prisma.user.create({
                data: { email: 'viewer@kiira.com', password: viewerPassword, role: 'viewer' }
            });
            console.log('✅ Viewer user created: viewer@kiira.com / viewer123');
        }
    } catch (error) {
        console.error('❌ Erro ao semear banco de dados:', error);
    }
}

// Rota de Login
app.post('/api/auth/login', async (req, res) => {
    const { email, password } = req.body;

    // Validação básica de entrada
    if (!email || !password) {
        return res.status(400).json({ error: 'E-mail e senha são obrigatórios' });
    }
    
    try {
        const user = await prisma.user.findUnique({ where: { email } });
        if (!user) {
            return res.status(401).json({ error: 'Usuário ou senha inválidos' });
        }

        const validPassword = await bcrypt.compare(password, user.password);
        if (!validPassword) {
            return res.status(401).json({ error: 'Usuário ou senha inválidos' });
        }

        const token = jwt.sign(
            { id: user.id, email: user.email, role: user.role },
            JWT_SECRET,
            { expiresIn: '24h' }
        );

        res.json({ token, role: user.role, email: user.email });
    } catch (error) {
        console.error(error);
        res.status(500).json({ error: 'Erro interno do servidor' });
    }
});

// Middleware para verificar JWT
const verifyToken = (req, res, next) => {
    const authHeader = req.headers['authorization'];
    const token = authHeader?.split(' ')[1];

    if (!token) return res.status(401).json({ error: 'Acesso negado' });

    jwt.verify(token, JWT_SECRET, (err, user) => {
        if (err) return res.status(403).json({ error: 'Token inválido' });
        req.user = user;
        next();
    });
};

// Rota para pegar info do usuário logado (usado pelo front-end para re-validar a sessão)
app.get('/api/auth/me', verifyToken, (req, res) => {
    res.json({ user: req.user });
});

// Exporta o App para o Vercel Serverless Function
module.exports = app;

// Roda localmente apenas se não estiver no ambiente de produção do Vercel
if (process.env.NODE_ENV !== 'production') {
    app.listen(PORT, async () => {
        console.log(`🚀 Servidor Backend JWT rodando na porta ${PORT}`);
        await seedDatabase();
    });
}
